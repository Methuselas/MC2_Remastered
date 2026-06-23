# SPIRV-KEYED-VARIANT-CONSUMER-1

> Generalizes the runtime SPIR-V consumer (SPIRV-CONSUMER-PILOT-BUILD-1) from
> *default-variant-only* to **keyed by define-set**, the prerequisite for the
> MechOpaque pilot (whose variant macros are default-ON). Merged to nifty
> (`ecdd9ece`). Default-OFF (`MC2_SHADER_SPIRV=1`), program-atomic preserved.

## What changed
**Before:** the consumer hard-rejected any prefix containing `#define` and looked
up a single `{base}.{stage}.default.json` sidecar — only the empty-define variant
could ever load SPIR-V.

**Now:** the runtime prefix's **complete** `#define` set is canonicalized into a
key and resolved against a deployed index:
- `spirvDefineKey(prefix)` (`shader_builder.cpp`): collects every `#define`
  (skips `#version`/`#extension`), normalizes each to `NAME=VALUE` (or `NAME`),
  **sorts**, joins with `;` — identical canonicalization to the offline builder.
- key = `base|stage|defkey`; resolved via `shaders/spv/spirv_index.json`
  (`{base|stage|defkey → artifact}`, written by `build_variants.py`).
- A **miss** (unknown program, or a define-set with no baked artifact) → `false`
  → `makeShader` returns null → `makeProgram2` rebuilds the whole program GLSL.
  **Watchpoint honored:** the key is the *complete* set, so any extra/missing
  define yields a different key and falls back — never a silent "some defines"
  partial match.

The `#define`-reject guard is removed from `spirvCompositePilotProgram` (which now
only decides which *programs* are pilots — still postprocess only this slice).
Program-atomic is unchanged: all stages of a program+variant load SPIR-V or the
whole program falls back to GLSL.

## Offline + CI
- `build_variants.py` emits `spirv_index.json` (records emit `"key"` before
  `"artifact"` so the C++ order-stable pairing works) + a `--check` index-sync
  verification. Canonical key kept in lockstep C++ ↔ Python.
- `check-spirv-artifacts.py` adds index-coverage (every pilot variant/stage has a
  matching `key→artifact` record). Registered seam check `spirv_artifacts`.

## Acceptance — all met (AMD 7900 XTX, postprocess pilot)
| Gate | Result |
|---|---|
| postprocess default ON/OFF | ✅ smoke PASS both; ON loads via key `postprocess|frag|` (0 fallbacks) |
| planted missing one-stage → atomic fallback | ✅ delete frag `.spv` → `rebuilding all stages GLSL`, composite intact |
| planted bad one-stage → atomic/fatal | ✅ corrupt frag → atomic GLSL rebuild |
| watchpoint: incomplete/extra define-set → fallback | ✅ tampered index key (`+MC2_FAKE=1`) → runtime key miss → atomic GLSL rebuild |
| hash/variant id matches offline metadata | ✅ index `variantId` == sidecar `shaderVariantId` (built from same `variant_id`); consumer logs the key |
| no mixed SPIR-V/GLSL links | ✅ program-atomic preserved |
| 7 seam checks | ✅ PASS (incl. index coverage) |
| full build green | ✅ RelWithDebInfo from-scratch |
| focused smoke OFF/ON | ✅ PASS |
| visual byte-diff | ✅ 2/2 deterministic bookmarks byte-identical OFF vs ON; `ridge_lowangle`/shadow_cascade excluded — **proven nondeterministic OFF-vs-OFF** (19c3c8d5 vs f3ad04b8), not the consumer |

## Notes
- `log_info` (`[SPIRV] no artifact for variant key…`) is not captured in smoke
  logs; the observable fallback signal is the `log_error` `rebuilding all stages
  GLSL`.
- Live non-empty define-set *success* selection lands with the MechOpaque pilot
  (the only program with non-empty default variants); the keyed mechanism is
  proven here via the empty key + the watchpoint (key-miss) path.

## Exclusions honored
No MechOpaque, no spec constants, no shader-system rewrite, no GLSL removal, no
Vulkan, no PSO cache. Foreign WIP (`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`)
untouched (md5 unchanged across merge).

## Next
`SPIRV-MECHOPAQUE-PILOT-BUILD-1` — add mech to the pilot-program allowlist; bake
`mech.{vert,frag}` variants (`MC2_OBJECT_ID_BUFFER` × `MC2_USE_VIEW_UNIFORMS`,
all meaningful combos); prove exact artifact selection (non-empty keys); visual
gate with a **frozen deterministic** mech frame.
