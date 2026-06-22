# SHADER-VARIANT-MATRIX-HARNESS-1

**Type:** Python, pure-parse. No GL, no glslangValidator, no engine.
**Policy:** WARN-first. Broad drift is reported, never fails; only named anchors hard-fail.
**Registered:** `tools/run_contract_tests.py` `PY_HARNESSES`.

## What it does

Cross-checks the two separately-authored sides of the shader variant system:

- **C++ side** — `#define NAME [VALUE]` strings injected into `makeProgram()`
  prefixes (`gameos_graphics.cpp:362/366/373/4898/4924/4965/5043`,
  `gos_postprocess.cpp:516/528`, `gos_terrain_lod_chunk.cpp:356`, plus all other
  injection sites across `GameOS/`, `mclib/`, `RenderCore/`).
- **GLSL side** — `#ifdef`/`#ifndef`/`#if defined()`/`#elif defined()` guards,
  and inline `#define` fallbacks, across `shaders/**`.

It classifies every macro and reports drift:

| Class | Meaning |
|---|---|
| covered | injected by C++ **and** guarded/defined in GLSL |
| WARN: guard used but never provided | a `#ifdef` whose macro nothing injects and no GLSL `#define` provides — dead branch or lost injection |
| WARN: injected but unreferenced | a C++-injected `#define` no shader guards or references — stale/typo injection |

## What it does NOT do

It does **not** compile shaders or validate binary layout (output locations,
bindings, std430 offsets) — that is `tools/shader_reflect/reflect.py`, which
compiles via glslangValidator + spirv-cross. reflect.py's `KNOWN_VARIANT_MACROS`
only spans `MC2_COALESCE` / `MC2_OBJECT_ID_BUFFER`. This harness is the
**complementary pre-compile symbol-graph check** for the broader injected↔guarded
macro set. It adds **visibility and regression coverage for variant-matrix
drift**; it does not change any shader, material, or variant behavior.

## Named anchors (hard regression tests)

These pairs were verified injected+referenced at authoring time; the test fails
if a future edit drops either side:

- `MRT_ENABLED` — injected + `#ifdef`-guarded
- `TERRAIN_NORMAL_ARRAY` — injected + `#ifdef`-guarded
- `MC2_SHADOW_CSM` — injected + `#ifdef`-guarded
- `MC2_SHADOW_CSM_MAX` — injected + GLSL-referenced (value macro; also has an
  `#ifndef ... #define MC2_SHADOW_CSM_MAX 3` fallback in `shaders/include/shadow.hglsl`)

## Current WARNs (known / triaged — NOT fixed by this slice)

- `ENABLE_TEXTURE1` — `#ifdef` in `gos_vertex*.frag`, never injected or defined →
  dead branch. (Matches CROSS-SUBSYSTEM-AUDIT-RECON-1 finding.)
- `ALPHA_TEST` — `#ifdef` in several shaders, driven by draw-call flags at
  dispatch, not by a compile-time prefix define → guard never compile-provided.

Both are pre-existing and out of scope here; the harness now makes them visible
and will flag any *new* such drift on future shader/prefix edits.

## Run

```
py -3 tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py
py -3 tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py --json
py -3 tools/run_contract_tests.py        # runs it alongside the other harnesses
```

`selftest_classifier` proves the parsers + classifier actually catch a planted
stale injection and a planted dead guard (fake-green guard); `demo_fail`
(via `--test demo_fail`) proves the exit-code path bites.
