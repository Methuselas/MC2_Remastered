# OFFLINE-SHADER-VARIANT-BUILD-1

> **Offline artifact pipeline only. No runtime change** — no `compile_shader()`
> branch, no `glSpecializeShader`, no hot-reload change, no spec-const conversion,
> no Vulkan. The runtime consumer is the next slice (`SPIRV-CONSUMER-PILOT-BUILD-1`).

## What shipped
Real, deployable **OpenGL SPIR-V** artifacts for the postprocess pilot family,
built offline from the same source the engine compiles — with **explicit bindings
preserved** (the runtime-correctness property reflect.py's auto-mapped `.spv`
lacks).

- **`tools/shader_offline_build/build_variants.py`** — generator. Per (pilot,
  variant, stage): `shader_common.build_shader_source` (version prefix + flattened
  `#include`s + inventory define-set) → `glslangValidator -G --auto-map-locations`
  (**NOT** `--auto-map-bindings`) → `.spv` → `spirv-cross --reflect` → sidecar JSON.
  `--check` verifies without writing.
- **`tools/shader_offline_build/pilots.json`** — pilot manifest. Start tiny: the
  postprocess composite pass (`postprocess.{vert,frag}`, `kShaderPrefix`, no variant
  macros, no UBO/SSBO blocks, location-based samplers, last-pass isolation).
- **`shaders/spv/`** — emitted artifacts (auto-deployed by `deploy_payload.py`'s
  recursive `shaders/` walk; no deploy change needed):
  - `postprocess.vert.default.24eb2442.spv` + `.json`
  - `postprocess.frag.default.24eb2442.spv` + `.json`
- **`scripts/check-spirv-artifacts.py`** — CI-cheap, tool-free verifier (registered
  in `check-contracts.sh` as `spirv_artifacts`).

## Why `-G --auto-map-locations` but NOT `--auto-map-bindings`
- `-G` = OpenGL SPIR-V (the flavor `GL_ARB_gl_spirv` / `glShaderBinary` consumes),
  not `-V` (Vulkan, which reflect.py uses for contract-checking).
- `--auto-map-locations` is required only for **inter-stage varyings** (e.g.
  `TexCoord`), which the GLSL has no explicit `location` on — locations are
  assigned deterministically by declaration order, consistent across stages.
- `--auto-map-bindings` is **deliberately omitted**: it would let glslang invent
  descriptor binding numbers that need not match the engine's real slots. The
  engine relies on explicit `layout(binding=)` + literal `glBindBufferBase`
  (binding-slot-occupancy). Omitting it forces explicit bindings to survive.
  (postprocess has **no** UBO/SSBO blocks, so there is nothing to drift here —
  which is exactly why it is the safest first pilot.)

## Sidecar metadata (per stage)
`{base, stage, variant, defines, specializationParams (empty), artifact,
source_sha256, spirv_sha256, bindings{ubos,ssbos,samplers}, interface{inputs,
outputs}, shaderVariantId}`. `shaderVariantId = sha256(base + "|" + sorted defines)[:12]`;
artifact name = `{base}.{stage}.{variant}.{vid8}.spv` (stable, variant-derived).

postprocess.frag reflected: 0 UBOs, 0 SSBOs, samplers `sceneTex`/`u_objectIdTex`
(location-based, default-uniform block — set at runtime via `glUniform1i`, not a
binding-base slot). Output `FragColor@0`, input `TexCoord@0`.

## Acceptance — all met
| criterion | result |
|---|---|
| offline build emits `.spv` for postprocess pilot | ✅ 2 artifacts (vert+frag) |
| explicit bindings preserved (no `--auto-map-bindings`) | ✅ (none to break; flag omitted) |
| sidecar metadata produced | ✅ base/stage/defines/specParams/bindings/hashes |
| reflection agrees with binding/sampler contracts | ✅ 0 UBO/SSBO bindings → no manifest conflict |
| planted binding drift fails | ✅ (fake UBO binding 99 → FAIL) |
| planted missing artifact fails | ✅ (removed `.spv` → FAIL) |
| planted source drift fails | ✅ (tampered `source_sha256` → FAIL) |
| GLSL runtime path untouched | ✅ no shader/runtime edits (new files only) |
| no relink required | ✅ (offline tooling + artifacts only) |

The verifier needs no external tools (recomputes the source hash with
`shader_common`), so it runs in `check-contracts.sh` alongside the other static
checks. Regenerate artifacts after a GLSL edit with `build_variants.py`.

## Next: `SPIRV-CONSUMER-PILOT-BUILD-1`
Default-OFF, extension-gated (`MC2_SHADER_SPIRV=1` + `GLEW_ARB_gl_spirv`/`VERSION_4_6`):
add the `compile_shader` SPIR-V branch (`glShaderBinary`+`glSpecializeShader`) for
the postprocess pilot; GLSL fallback on unsupported extension or missing artifact;
hot-reload stays GLSL; visual-equality proof. Then widen the pilot manifest
(MechOpaque next) and convert `MC2_SHADOW_CSM_MAX` to a real `layout(constant_id)`.

## Exclusions honored
No `compile_shader()` runtime branch, no `glSpecializeShader`, no shader edits, no
hot-reload change, no spec-const conversion, no Vulkan, MechOpaque deferred.
Foreign WIP (`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.
