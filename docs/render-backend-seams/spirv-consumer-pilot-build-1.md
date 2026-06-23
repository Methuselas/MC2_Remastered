# SPIRV-CONSUMER-PILOT-BUILD-1

> **First runtime-touching shader slice.** Built in isolated worktree
> `A:/Games/mc2-spirv-consumer` @ `claude/spirv-consumer-pilot-1` (off nifty
> `229af093`). Default-OFF, extension-gated. NOT yet merged to nifty.

## What shipped
A runtime SPIR-V consumer for the **postprocess composite** pilot. When
`MC2_SHADER_SPIRV=1` AND the driver exposes SPIR-V, the composite
(`postprocess.vert` + `postprocess.frag`) loads the offline-baked `.spv`
(OFFLINE-SHADER-VARIANT-BUILD-1) via `glShaderBinary` + `glSpecializeShader("main")`
instead of compiling GLSL. Every other shader, and every fallback case, stays
on the unchanged GLSL path.

### Gate (all must hold)
`MC2_SHADER_SPIRV=1` · `GLEW_ARB_gl_spirv || GL 4.6` (the 4.3 context request
does NOT imply support — checked at runtime) · NOT `MC2_SHADER_HOT_RELOAD` ·
the program is the composite pilot pair · default variant (no `#define` prefix).

### Program-atomic (the key correctness property)
`postprocess.vert` is shared by cloud/shoreline/ssao/ssao_apply/fog_oob/edge_fog/
hzb_reduce. A per-file decision loaded SPIR-V for the vert in those programs while
their frag stayed GLSL → **SPIR-V/GLSL mixed link FAILS** (first smoke ON showed 7
`failed to compile` lines). Fixed: the decision lives in `makeProgram2`
(`spirvCompositePilotProgram`), matching ONLY the exact composite pair. A program
is therefore all-SPIR-V or all-GLSL — never mixed. If any pilot stage's SPIR-V
fails to load, `makeShader` returns null and `makeProgram2` rebuilds **every**
stage as GLSL (atomic fallback).

### Fallback matrix (all → GLSL, logged; never silent)
env OFF · extension absent · artifact missing · corrupt/rejected `.spv` ·
specialize failure · hot-reload active · non-pilot shader · non-default variant.
`MC2_SHADER_SPIRV_FATAL=1` asserts/logs instead of falling back (debug).
`reload()` is GLSL-only by construction, so hot-reload is always GLSL.

## Acceptance — all met (AMD Radeon RX 7900 XTX, 4.3 Core ctx, GLEW_ARB_gl_spirv)
| Gate | Result |
|---|---|
| Env OFF → GLSL path unchanged | ✅ tier1 5/5, 0 SPIR-V activity |
| Env ON + ext present → postprocess loads SPIR-V | ✅ proven: OFF=0 / old-ON=7 / fixed-ON=0 mixed-link differential; `glShaderBinary` reached composite (FATAL probe) |
| Missing `.spv` → GLSL fallback, no crash | ✅ atomic rebuild, 0 broken composite |
| Bad `.spv` → GLSL fallback or fatal gate | ✅ corrupt → atomic GLSL rebuild; FATAL gate aborts-path verified |
| Hot reload → still GLSL | ✅ `reload()` never touches SPIR-V |
| Full build green | ✅ RelWithDebInfo |
| Tier1 smoke OFF | ✅ 5/5 |
| Tier1 smoke ON | ✅ 5/5, 0 postprocess compile failures (all 5 missions) |
| Visual proof byte/pixel-identical | ✅ mc2_01 3/3 bookmarks BYTE-IDENTICAL OFF vs ON (overview_center/ridge_lowangle/highangle_wide) |
| 7 seam contract checks | ✅ binding_slots, sampler_bindings, material_gpu_mirror, pipeline_desc, shader_injectors, pipeline_key, spirv_artifacts |

## Files
- `GameOS/gameos/utils/shader_builder.{cpp,h}` — `trySpirvSpecialize()` +
  `spirvCompositePilotProgram()` + `makeShader(trySpirv)` + `makeProgram2`
  atomic gate/fallback.
- `RenderCore/RendererFeatureRegistry.h` — `MC2_SHADER_SPIRV`, `MC2_SHADER_SPIRV_FATAL`.
- Portability fix to OFFLINE-SHADER-VARIANT-BUILD-1: `canon_source()` (strip
  `#line` path labels + normalize newlines) in `build_variants.py` +
  `check-spirv-artifacts.py` — `source_sha256` was path-dependent and drifted
  across worktrees; rebuilt artifacts (portable hash, identical `.spv`).

## Notes / limits
- `glProgramName`/variant identity stays runtime (descriptive) — promotion to a
  real keyed cache is future work.
- Stale-`.spv` guard is build/deploy lockstep (`check-spirv-artifacts.py` +
  deploy ships in step) + the pilot allowlist + opt-in; there is no runtime
  source-hash recompute (the engine can't cheaply reproduce the Python hash).
- Worktree-only; deployed/tested in `mc2-win64-v0.4c`. Merge to nifty (which
  needs its own relink and is multi-session-live) is a deliberate follow-up.

## Exclusions honored
No MechOpaque, no spec constants, no global shader-system rewrite, no GLSL
runtime removal, no Vulkan, no PSO cache, no shader source edits. Foreign WIP
(`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.

## Next
`SPIRV-MECHOPAQUE-PILOT-RECON-1` — MechOpaque is the first registered/keyed
pipeline family with real variants (OBJECT_ID × USE_VIEW_UNIFORMS); recon the
per-variant bake + the dedicated (non-shared) vertex layout before building.
