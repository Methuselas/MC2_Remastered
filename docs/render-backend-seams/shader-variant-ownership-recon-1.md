# SHADER-VARIANT-OWNERSHIP-RECON-1 (banked)

Read-only recon, banked to unblock a future shader-variant/program ownership arc.
Source-verified vs nifty `69a738b5`.

## How programs are built + varied

- `makeProgram2` / `makeProgram` (`GameOS/gameos/utils/shader_builder.cpp:976/1163`) build a
  GL program from per-stage `makeShader`; a **prefix string** (`#version 430\n` + extensions
  + `#define`s) is prepended before `glShaderSource`. Variants are expressed entirely by the
  prefix define-set.
- **Variant axes (defines):** `MC2_OBJECT_ID_BUFFER` (gate `RenderWorld::IsObjectIdBufferEnabled`),
  `MC2_USE_VIEW_UNIFORMS` (default ON), `MC2_COALESCE` (+`GL_ARB_shader_draw_parameters`,
  `IsCoalesceEnabled`), `MC2_STATICPROP_PBR_SLOTS`, `MC2_IMPORTED_MECH_MATERIAL`.
- **Runtime program resolvers:** `resolvePropShadowProgram` (`gos_static_prop_batcher.cpp:7365`,
  alpha-vs-empty by `MC2_SHADOW_PROP_ALPHA`), `IsCoalesceEnabled` (1459, legacy vs coalesce
  program), `loadProgramsIfNeeded` (static-prop 1097 / mech 547).
- **Variant key:** `glsl_program::shaderDefineKey` parses+sorts the prefix into a canonical
  `NAME=VAL;...` key, recorded via `recordPipelineVariantKey` and emitted as
  `[PIPELINE_VARIANT] pipeline=.. glProgram=.. key=.. vertexLayout=..`.

## SPIR-V

- Pilot families (`shader_builder.cpp:448`): postprocess, mech, ssao, cloud, shoreline,
  fog_oob, edge_fog, hzb_reduce. Gate `MC2_SHADER_SPIRV=1` (default OFF) + no hot-reload +
  `GL_ARB_gl_spirv`/4.6. **Program-atomic fallback:** any stage SPIR-V miss → whole program
  rebuilt as GLSL (no mixed program). Index `shaders/spv/spirv_index.json`; package metadata
  `spirv_package.json`. `check-spirv-artifacts.py` + `check-spirv-reflection-contract.py`
  already govern artifact presence/reflection.
- bindProgram wires `PipelineDesc.glProgramName` (`PipelineRegistry.cpp:505`); most non-batcher
  rows keep glProgramName=0 (descriptive; call site binds).

## The load-bearing invariant (cleanest first target)

**Depth-prepass program variant MUST match the color program's variant**
(`gos_static_prop_batcher.cpp:1197-1203` + depth setup): both must reflect the same
`IsCoalesceEnabled` decision, or the depth-prepass lays depth a `GL_EQUAL` color pass can't
match → silent depth failure in complex scenes. Today the decision is evaluated separately
for color and depth; if it drifts between phases the variants mismatch.

→ **First hardening:** factor the variant decision to one pure function returning
`(programName, prefix)`, call it once for both color+depth, and a checker that asserts the
depth pipeline's recorded variantKey == the color pipeline's. A `check-shader-variant.py`
could parse the `[PIPELINE_VARIANT]` records / `recordPipelineVariantKey` sites and assert
color↔depth lockstep + that every define-set reaching makeProgram has a SPIR-V index entry
or an explicit GLSL-only marker.

## What a shader-variant ownership ledger/checker would govern
Program identity per pipeline (glProgramName ↔ variantKey ↔ feature bits); variant-matrix
completeness (every reachable define-set has an index entry or GLSL-only verdict);
prefix↔spirv_index key consistency; color/depth variant lockstep; resolver decision audit.

## Verdict
Medium-value, safe (mostly descriptive + a lockstep checker). The depth/color variant
lockstep is a genuine correctness guard worth building first. Lower urgency than class-D
perf but cleaner/safer than barrier work.
